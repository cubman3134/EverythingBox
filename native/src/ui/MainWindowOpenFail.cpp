// THE FAILED OPEN THAT STAYS PUT (issue #239) — MainWindow's half.
//
// A press that ends without opening anything used to leave a toast and nothing else. Four seconds later the
// shelf looked exactly as it had before the press: no message, no mark, no trace of the attempt. That is how
// #236 came to be filed as "the shelf's Play never reaches openGame", with the emulator backend suspected,
// when the real event was a download that came back empty and said so briefly.
//
// The store (core/OpenFailStore) holds the fact; this file is the three verbs the window needs over it —
// write it, forget it, repaint whatever is showing the item — and it lives in its own translation unit for
// the reason MainWindowPlayOn.cpp does (#143/#186): MainWindow.cpp is the single busiest merge surface in
// the repository, and a feature that can be a file of its own should not be another hunk in the middle of it.
//
// WHAT IS DELIBERATELY NOT HERE. The reporting itself: MainWindow::reportOpenFailure is the one funnel every
// failed open passes through, and noteOpenFailure is called from INSIDE it rather than beside it, so a new
// failure shape cannot reach the screen and miss the store. And the CLASSIC page's repaint, which HomeView
// owns end to end — it builds those widgets, so it is the only thing that can honestly re-render them.
#include "MainWindow.h"
#include "HomeView.h"

#include "../core/AppPaths.h"
#include "../core/OpenFailStore.h"

#include <QDateTime>
#include <QFile>
#include <QStackedWidget>

#ifdef EB_HAVE_QML
#include "../theme2/ThemeEngine.h"
#include <QQuickItem>
#include <QVariantMap>
#endif

namespace {

// The same one-line append to <app>/stream_debug.log that MainWindow.cpp's mwLog does. Copied rather than
// shared because mwLog is a file-static there and lifting it out would touch that file for no other reason;
// five lines is the cheaper trade. It matters that these lines exist at all: stream_debug.log is where #236
// was diagnosed from, and "no game: line was written" is only half a story without "and here is what was
// written instead".
void ofLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QStringLiteral("\n")).toUtf8());
}

} // namespace

void MainWindow::noteOpenFailure(const OpenFailSubject& subject, const QString& message)
{
    // No id, nothing to key on — and nothing dishonest done about it. A failure with no catalog row in hand
    // (mpv refusing a pasted link, a local file that has gone) still gets its toast from the funnel above;
    // what it does not get is a mark on somebody else's row. Falling back to the TITLE here is exactly the
    // bug probe_openfail §2 exists to forbid: two dumps of one game share a title, and marking the wrong one
    // tells the user the copy that works is the broken one.
    if (subject.id.isEmpty() || message.isEmpty()) return;

    OpenFailStore::record(subject.id, subject.title, message);
    ofLog(QStringLiteral("openfail: recorded for \"%1\"")
              .arg(subject.title.isEmpty() ? subject.id : subject.title));
    // The detail page may be the very surface the press was made from, so it has to grow its banner and its
    // verbs without a navigation.
    refreshOpenFailureSurfaces();
    if (home_) home_->refreshOpenFailureMarks();
}

void MainWindow::clearOpenFailure(const QString& itemId)
{
    if (itemId.isEmpty()) return;
    // Asked before writing, so an ordinary successful open — which is nearly all of them — costs one lookup
    // and no file write at all. openFrom() runs this on EVERY open, not only on the ones that follow a
    // failure, precisely so that it does not have to know which is which.
    if (!OpenFailStore::marked(itemId)) return;

    OpenFailStore::clear(itemId);
    ofLog(QStringLiteral("openfail: cleared for \"%1\"").arg(itemId));
    refreshOpenFailureSurfaces();
    if (home_) home_->refreshOpenFailureMarks();
}

void MainWindow::refreshOpenFailureSurfaces()
{
#ifdef EB_HAVE_QML
    // Themed only — the classic page repaints through HomeView::refreshOpenFailureMarks, which owns those
    // widgets.
    //
    // NOT through HomeView's browseItemsChanged, which is what this reached for first and what the live
    // drive threw out. That signal means "a load, drill or page moved my stack": its handler mirrors the
    // stack onto the nav graph, forces currentView back to "browse" and re-selects browseRestoreIndex() —
    // so using it to repaint one badge closed the detail page the press had been made from and threw the
    // cursor back to the first tile. What is wanted is much smaller, and MainWindow is the only side that
    // can tell the two cases apart.
    if (!home_ || !stack_) return;
    QQuickItem* r = ThemeEngine::rootItem(stack_->currentWidget());
    if (!r) return;

    if (themedDetailIndex_ >= 0 && r->property("currentView").toString() == QStringLiteral("detail"))
    {
        // The detail page is the surface. Re-push the WHOLE of themedDetailData rather than patching the two
        // keys, because a failure changes the verb LIST as well as the banner (the "Try again" and "Dismiss"
        // pills appear and disappear with it) and a patched map would leave the ActionRow's Repeater bound to
        // a stale one — the idiom the status-pill branch of runThemedDetailAction already uses.
        r->setProperty("detailData", home_->themedDetailData(themedDetailIndex_));
        // ...and the row BEHIND the page re-reads on the way out, which is Hide's mechanism (#65/#24) and is
        // here for its reason too: re-sourcing the grid's model while its detail view is open is the #28
        // shape, and the row is not visible until the pop anyway.
        themedDetailMarksDirty_ = true;
        return;
    }
    // No detail page: the shelf itself may be on screen (the common case — the press was made from it), so
    // re-source the grid in place and put the cursor back where it was.
    //
    // GATED ON THE BROWSE VIEW, which is not a formality. On a theme whose home is the XMB cross, the SAME
    // root item is showing its category list, and `items` there is that list — writing browseItems() onto it
    // would replace the user's categories with whatever catalog HomeView happens to be holding. The XMB
    // column draws no badge anyway (that is Grid.qml's card), so there is nothing to repaint.
    if (r->property("currentView").toString() != QStringLiteral("browse")) return;
    const int keep = r->property("currentIndex").toInt();
    r->setProperty("items", home_->browseItems());
    r->setProperty("currentIndex", keep);   // clamped by the QML model if it now overshoots
#endif
}
